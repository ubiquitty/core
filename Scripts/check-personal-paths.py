#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Scripts/check-personal-paths.py — public-tree privacy lint.

BLOCKS on machine-specific personal identifiers that must not ship in a public
tree.

The rule matches a SHAPE and then subtracts the values that are permitted. It
names no person, which is the whole design: a rule carrying one person's
username, hostname and account as literals blocks exactly one identity — and CI
runs as 'runner' and knows nobody else — so any other contributor's home path
passes the release gate. Matching the shape protects
everyone identically and survives the maintainer, the contributors and the CI
account all changing.

Two patterns rather than one, because one pattern excluding the permitted values
needs a negative lookahead, and a lookahead in an ERE does not error — it changes
the meaning and the rule stops matching, silently.

  DETECT   the shape of a home path, an address, a personal machine name
  ALLOW    the values that are synthetic, published, or a third party's own

Anything DETECT finds and ALLOW does not cover is a violation. An ALLOW entry is
therefore a claim that a value is already public, so this list holds nothing that
needs protecting.

The DERIVED half stays: the current user (id -un) and short hostname
(hostname -s), which catch an identity whose shape says nothing — a username that
looks like any other word.

Test fixtures use neutral synthetic values (/Users/user, /home/user, user@host);
the public remote is github.com/yaglo/ubiquitty.

Derived-half inputs (defaults: id -un, hostname -s):
  --user <name> / --host <name>       explicit override, before other args
  UBIQUITTY_LINT_USER / UBIQUITTY_LINT_HOST   environment equivalents

EXEMPT (not scanned):
  - Source/ThirdParty/               vendored upstream code
  - Tests/Core/fuzz-corpus/          opaque fuzz inputs (binary; scrubbed separately)
  - binary fixtures (png/ttf/…) and this script itself.
Published contact (yaglo@me.com in CODE_OF_CONDUCT/SECURITY/author headers)
matches no pattern here and is preserved.

Usage:
  Scripts/check-personal-paths.py            # all tracked files
  Scripts/check-personal-paths.py --staged   # staged files only (pre-commit)
  Scripts/check-personal-paths.py file ...   # specific files
  Scripts/check-personal-paths.py --self-test # attribution scope controls

Exit 0 = clean, 1 = personal-identifier violations, 2 = usage error.

Wired into Scripts/hooks/pre-commit (BLOCKS) and Scripts/check-style.py.

The detect pattern runs once over each whole file rather than once per line:
almost no file carries a hit, so a clean file costs the regex engine one entry.
"""

import os
import re
import socket
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import gate_report

GATE = "check-personal-paths"

# Kept for the message and the tests: what the gate asks authors to write.
NEUTRAL_HINT = "/Users/user, /home/user, user@host"

# Vendored code, opaque corpora, binaries, and this lint itself are exempt.
EXCLUDE = (r"(^|/)(Source/ThirdParty/|Tests/Core/fuzz-corpus/)"
           r"|\.(png|jpg|jpeg|gif|ico|pdf|zip|gz|woff2?|ttf|otf|dat|bin)$")

# A derived name that names nobody. A shared system account (root, runner, the CI
# builder) names the machine's role, and a short generic hostname (vm, ci, box)
# is a word before it is a name — a two-letter host turns `\bvm\b` into a match
# on every `vm` identifier in C++. Such a token is DROPPED from DETECT and named
# on stderr, because a half that silently stopped looking must not read as one
# that looked and found nothing.
GENERIC_IDENTITIES = re.compile(
    r"^(root|admin|administrator|user|runner|build|builder|jenkins|ci|container"
    r"|docker|vagrant|ubuntu|debian|vm|box|localhost|unknown-user"
    r"|unknown-host)$",
    re.IGNORECASE)

# DETECT names a shape, never a person.
#
# Case follows the filesystem, which is why the two halves differ. A macOS volume
# is case-insensitive and case-preserving, so a home path spelled in any casing
# reaches the same directory — a real, working path, and matching only the
# canonical spelling misses it. Linux is case-sensitive, so the `home` branch
# stays exact: folding it matches the Home and End key names in a comment about
# the cursor keys, which is not a path at all.
DETECT_HOME = r"/((?i:Users)|home)/[A-Za-z0-9._-]+"
# A backslash cannot be a bare separator: it matches every C escape a terminal
# emulator's tests are full of, and a directory-setting escape sequence became a
# finding when it was allowed. So the Windows form is anchored on what only a
# Windows path has — a drive letter, or a UNC host — which no escape sequence
# carries.
DETECT_WINHOME = (r"([A-Za-z]:|\\\\[A-Za-z0-9._-]+)"
                  r"\\(?i:Users)\\[A-Za-z0-9._-]+")
# `~name/` is a home path and `~word/2` is "approximately". They are the same
# shape, so the split is on what follows: a division names a number on both
# sides. Measured against this tree — `~gapTotal/2`, `~leading/2`, `~1.4s/sweep`
# are every tilde in it, and each has a digit on one side or the other.
DETECT_TILDE = r"~[A-Za-z][A-Za-z0-9._-]+/[A-Za-z._-]"
DETECT_MAIL = r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"
# A personal machine names itself after its model. Structural because a bare
# token cannot be recognised: a rule carrying a machine's bare name, such as
# `\bmba\b`, blocks that word anywhere in the tree — in prose, in an identifier,
# in a base64 blob — for one machine.
#
# A bare `\.local` is left out although it is the usual hostname suffix: it
# matches `path.local` in C++ member access, 159 hits in Source/ alone. The model
# names below carry a hyphen, which an identifier cannot.
#
# Case-insensitive: macOS builds the Bonjour name from the Computer Name and
# keeps its capitals, so the shipped spelling carries them and the lowercase
# alternation alone would miss every unmodified Mac.
DETECT_HOST = (r"[A-Za-z0-9]([A-Za-z0-9-]*)-"
               r"(?i:macbook|macbookpro|mbp|imac|macmini|laptop|desktop)")

# ALLOW is anchored, because it judges a whole extracted match rather than a
# line. An entry here is a claim that the value is synthetic, published, or a
# third party's own licence header — each is public by construction, so this list
# holds no private information and needs no protection of its own.
ALLOW_HOME = (r"^/((?i:Users)|home)/(user[0-9]*|usertwo|u|x|me|you|someone"
              r"|somebody|runner|root|shared|Shared|linuxbrew|ubq-transport)$")
# The same neutral names in the Windows spelling, so the two forms agree about
# what is synthetic.
ALLOW_WINHOME = (r"^([A-Za-z]:|\\\\[A-Za-z0-9._-]+)\\(?i:Users)\\(user[0-9]*"
                 r"|usertwo|u|x|me|you|someone|somebody|runner|root|shared"
                 r"|Shared)$")
# The tilde spelling of the same neutral names. DETECT_TILDE takes one character
# past the slash to tell a path from a division, so this matches it.
ALLOW_TILDE = (r"^~(user[0-9]*|usertwo|u|x|me|you|someone|somebody|runner"
               r"|root)/[A-Za-z._-]$")
# Reserved-for-documentation domains (RFC 2606 / RFC 6761) are synthetic by
# definition. The rest are enumerated: two published project contacts, three
# upstream authors named in vendored licence text, and the fixtures.
ALLOW_MAIL = (r"^[A-Za-z0-9._%+-]+@([A-Za-z0-9.-]+\.)?(example\.(com|net|org"
              r"|test|invalid)|example|invalid|test|localhost)$"
              r"|^(yaglo@me\.com|yaglo@users\.noreply\.github\.com)$"
              r"|^(smueller@chronox\.de|orsonpeters@gmail\.com"
              r"|eay@cryptsoft\.com)$"
              r"|^(user@host\.example|user@host\.local|grace\+ci@host\.example"
              r"|deploy@build-07\.internal|a@b\.com|t@example\.com)$")
ALLOW_HOST = r"^(host|build-07|localhost|user)\.local$"

# Each notice records the upstream source for these published attributions.
# Splitting mailbox and domain keeps the rule's source subject to the same scan.
# Only the exact address in its exact notice is exempt; neighboring matches are
# checked independently, including other addresses in that notice.
PUBLISHED_ATTRIBUTIONS = {
    "Docs/third-party-licenses/Theme-LICENSES.txt": (
        ("me", "kons.ch"),
        ("development", "svengreb.de"),
        ("morhetz", "gmail.com"),
    ),
    "Docs/third-party-licenses/swift-crypto-4.5.2-CONSTITUENTS.txt": (
        ("andreser", "mit.edu"),
        ("jadep", "mit.edu"),
        ("jade.philipoom", "gmail.com"),
        ("zoe.paraskevopoulou", "gmail.com"),
    ),
    "Resources/fonts/SymbolsNerdFont-CONSTITUENTS.txt": (
        ("davidebignotti", "gmail.com"),
        ("gabriele.lana", "gmail.com"),
    ),
}

# Machine names the maintainer approved for publication in one file. The derived
# half matches the running machine's short hostname, so on the machine a
# published capture shows, its name in that capture's caption is a finding there
# and nowhere else. Only the exact name in the exact file is exempt.
PUBLISHED_HOSTS = {
    "README.md": ("hub",),
}

# The table above and the test that exercises it spell every approved name, so
# on the machine of that name its derived half would refuse them, and with them
# every push made there.
PUBLISHED_HOSTS_SPELLED_IN = (
    "Scripts/check-personal-paths.py",
    "Scripts/test-personal-paths.py",
)


def escape_regex(value):
    """Regex metacharacters in a derived name, escaped.

    A hostname like "mac.local" must not become a wildcard. The shell half used
    a sed class; this uses the same set so both spell the same pattern.
    """
    return re.sub(r"([][\\.|$(){}?+*^])", r"\\\1", value)


def derived_identity():
    """The current user and short hostname, with a floor under each.

    id -un and hostname come back empty in a container whose uid is not in
    passwd, and an empty value turns "$LINT_USER@" into a bare "@" that matches
    every seed. Both get a fallback, covering the case where the command exits
    zero with no output.
    """
    user = os.environ.get("UBIQUITTY_LINT_USER")
    if not user:
        finished = subprocess.run(["id", "-un"], capture_output=True,
                                  text=True, check=False)
        user = finished.stdout.strip()
    host = os.environ.get("UBIQUITTY_LINT_HOST")
    if not host:
        host = socket.gethostname().split(".")[0]
    return user or "unknown-user", host or "unknown-host"


def is_generic(value):
    """Whether a derived name names a role rather than a person."""
    return bool(GENERIC_IDENTITIES.match(value)) or len(value) < 3


def patterns(user, host):
    """The DETECT and ALLOW pair, assembled the way the shell half assembled it.

    - Returns: (detect, allow) as pattern text, not compiled — Tests compare the
      text against the shell spelling, and a compiled object cannot be compared.
    """
    user_pattern = escape_regex(user)
    host_pattern = escape_regex(host)
    derived = ""
    if is_generic(user):
        print(f"{GATE}: derived user '{user}' is generic — the user half is not "
              f"scanned (shape halves still are)", file=sys.stderr)
    else:
        derived = (rf"|/((?i:Users)|home)/{user_pattern}\b"
                   rf"|\b{user_pattern}@")
    if is_generic(host):
        print(f"{GATE}: derived host '{host}' is generic — the host half is not "
              f"scanned (shape halves still are)", file=sys.stderr)
    else:
        derived = derived + rf"|\b{host_pattern}\b"
    detect = (f"{DETECT_HOME}|{DETECT_WINHOME}|{DETECT_TILDE}|{DETECT_MAIL}"
              f"|{DETECT_HOST}{derived}")
    allow = (f"{ALLOW_HOME}|{ALLOW_WINHOME}|{ALLOW_TILDE}|{ALLOW_MAIL}"
             f"|{ALLOW_HOST}")
    return detect, allow


def violations(text, detect, allow, published=()):
    """Matches of detect the allow list does not cover, in the order they appear.

    Filtered per MATCH, not per line: a line may carry a permitted value and a
    forbidden one at once (a comment naming /Users/user beside a real path), and
    suppressing the whole line because something on it was allowed is how an
    exemption grows to cover its neighbours.
    """
    return [match for match in detect.finditer(text)
            if not allow.search(match.group(0)) and match.group(0) not in published]


def scan(paths, detect, allow, unreadable):
    """Findings across the corpus, one header per line followed by its matches.

    A binary file is scanned too — a committed artifact must be as clean as
    source — but reports the FILENAME only: echoing a raw binary line corrupts
    any text-mode consumer of this output.
    """
    report = []
    for path, data, binary in gate_corpus.contents(paths, unreadable):
        relative_path = os.path.relpath(os.path.realpath(path), os.getcwd())
        published = {f"{mailbox}@{domain}" for mailbox, domain
                     in PUBLISHED_ATTRIBUTIONS.get(relative_path, ())}
        published |= set(PUBLISHED_HOSTS.get(relative_path, ()))
        if relative_path in PUBLISHED_HOSTS_SPELLED_IN:
            published |= {host for hosts in PUBLISHED_HOSTS.values() for host in hosts}
        if binary:
            bad = violations(data.decode("latin-1"), detect, allow, published)
            if bad:
                report.append(f"  {path}: binary file contains a personal "
                              f"identifier ({bad[0].group(0)})")
            continue
        text = data.decode("utf-8", "replace")
        # One pass over the whole file. A clean file leaves here having run the
        # engine once rather than once per line, which is where the time went.
        hits = violations(text, detect, allow, published)
        if not hits:
            continue
        starts = gate_corpus.line_starts(text)
        grouped = []
        index = 0
        for match in hits:
            number, index = gate_corpus.line_of(starts, match.start(), index)
            header = f"  {path}:{number}: {gate_corpus.body_at(text, starts, index)}"
            if not grouped or grouped[-1][0] != header:
                grouped.append((header, []))
            grouped[-1][1].append(match.group(0))
        for header, values in grouped:
            report.append(header)
            report.extend(f"      -> {value}" for value in values)
    return report


def corpus_seeds(user):
    """Fuzz seeds carrying the derived user, by filename.

    Fixed strings, not patterns: the derived values are data, and a hostname can
    carry a `+`, a `.` or a `-`. Building a search pattern out of a name is how a
    guard silently matches nothing while reporting a clean result.

    Only the path- and email-contextualised forms. A bare account name is an
    ordinary word in opaque data and matches by chance: run under CI's generic
    account, the bare name "builder" matched 52 of these seeds as plain English,
    while the same name inside a home path or an address matched none.
    """
    needles = [f"/Users/{user}".lower(), f"/home/{user}".lower(),
               f"{user}@".lower()]
    found = []
    for directory, _, names in os.walk("Tests/Core/fuzz-corpus"):
        for name in sorted(names):
            path = os.path.join(directory, name)
            try:
                with open(path, "rb") as handle:
                    body = handle.read().lower()
            except OSError:
                continue
            if any(needle.encode("utf-8", "replace") in body
                   for needle in needles):
                found.append(path)
    return found


def main(argv):
    arguments = list(argv)
    if arguments == ["--self-test"]:
        return subprocess.call([sys.executable, os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "test-personal-paths.py")])
    user, host = derived_identity()
    while arguments:
        if arguments[0] in ("--help", "-h"):
            print(__doc__.strip())
            return 0
        if arguments[0] == "--user":
            if len(arguments) < 2:
                print(f"{GATE}: --user needs a value", file=sys.stderr)
                return 2
            user, arguments = arguments[1], arguments[2:]
        elif arguments[0] == "--host":
            if len(arguments) < 2:
                print(f"{GATE}: --host needs a value", file=sys.stderr)
                return 2
            host, arguments = arguments[1], arguments[2:]
        else:
            break

    os.chdir(gate_corpus.root())
    detect_text, allow_text = patterns(user, host)
    detect = gate_corpus.compiled(detect_text, GATE, "detect")
    allow = gate_corpus.compiled(allow_text, GATE, "allow")

    paths = gate_corpus.select(arguments, exclude=EXCLUDE)
    if not paths:
        print(f"{GATE}: no files to scan")
        return 0

    unreadable = gate_corpus.Unreadable()
    hits = scan(paths, detect, allow, unreadable)
    seeds = corpus_seeds(user)

    print()
    # Flushed before the verdict goes to stderr. A caller that captures the gate
    # with `> out 2>&1` — which is how Scripts/check-style.py captures every one
    # of them — sees the two streams in the order they were written only if the
    # buffered one is emptied first.
    sys.stdout.flush()
    if hits or seeds:
        # Count the findings, not the lines: a text hit prints its source line
        # and then the value that matched, so counting lines reports double —
        # count only the "  file:" heading. A corpus hit is one filename per line
        # and adds one each; without this it counted zero of them, so a real
        # corpus leak reported "0 identifier(s)" beside a non-empty list.
        headings = sum(1 for line in hits
                       if line.startswith("  ") and not line.startswith("   "))
        gate_report.failed_count(
            GATE, headings + len(seeds),
            "personal identifier(s) in the public tree, listed below")
        sys.stderr.flush()
        for line in hits:
            print(line)
        if seeds:
            print("  fuzz-corpus seeds (binary):")
            for path in seeds:
                print(f"    {path}")
        print()
        print(f"  Use a neutral synthetic value: {NEUTRAL_HINT}.")
        print("  The public remote is github.com/yaglo/ubiquitty. Bypass once with:")
        print("    Remove the absolute path, or make it relative to the repo root.")
        return 1
    scanned = f"{len(paths)} file(s) scanned"
    if unreadable:
        scanned = f"{scanned}, {len(unreadable)} unreadable"
    print(f"{GATE}: ok ({scanned})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
