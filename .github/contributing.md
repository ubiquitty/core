<!-- SPDX-FileCopyrightText: 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Contributing to ubiquitty-core

Thanks for your interest in Ubiquitty. This repository is libubiquitty, the
portable C++23 terminal core, and the multiplexer built on it: the headless
`ubiquitty-host` daemon, Sync, the agent and the `ubq` command. Renderers and
terminal views are in `ubiquitty-kit`; the apps are in `ubiquitty`.

## Getting started

Start with [`Docs/architecture.md`](../Docs/architecture.md) for the layer map
and end-to-end pipeline, then read
[`Docs/coding-conventions.md`](../Docs/coding-conventions.md) for how the code
is written. The one rule everything hangs on: **no
platform-specific API (Cocoa/Metal/AppKit/GTK/Vulkan) in Core or any shared,
host-tested translation unit** — platform code lives behind the PAL seam.

The hooks run the naming, formatting and boundary checks at commit time, so a
violation is caught before it reaches a pull request. Install them once per
clone:

```sh
Scripts/install-hooks.sh
```

### Choose work

Open issues are the work available to contributors. Ones labelled
`good first issue` are scoped to be completable without knowing the whole tree.
Before starting something substantial, open an issue describing the change and
wait for a reply — the answer may be that it is already in progress, and that
conversation is cheaper than a rejected pull request.

Branch from `main` and name the branch for the change. Fork the repository
unless you have push access.

Repository hooks run the checks shipped here. A clone can add checks of its
own: the pre-commit and pre-push hooks also run every executable file in the
clone's `.git/hooks.d/pre-commit/` and `.git/hooks.d/pre-push/`
([`run-local-hooks.sh`](../Scripts/run-local-hooks.sh)), and a clone with none
runs none. A maintainer can protect a shared checkout with
`UBIQUITTY_SHARED_CHECKOUT`; ordinary clones are writable.

### Build & test

Git, Python 3, CMake, Bash, and a C++23 toolchain are what every build needs.
The first QUIC-enabled build, which includes the host, also needs network
access, Go, and Perl (`brew install go` on macOS; Perl ships with it).

```sh
./Scripts/build-host.sh       # the headless ubiquitty-host daemon
./Scripts/build-agent.sh      # ubq, the headless client and MCP server
./Scripts/build-tests.sh      # compiles every test, runs none
./Scripts/run-tests.sh        # the host-buildable test suite
./Scripts/run-tests.sh --list-stanzas # the shell stanza names run-tests.sh accepts
./Scripts/check-style.py      # every content gate on this branch's changes; --full for the whole tree
./Scripts/run-linux.sh <command...>   # the same build in a Linux container
```

A C++ unit test such as `Tests/Core/VT/test_vt100_cursor.cpp` is a CTest case,
not a stanza, so `run-tests.sh` does not select it by name. Configure the test
tree once, list its cases, and run one by exact name:

```sh
CTEST_BUILD_DIR="$(python3 Scripts/build_root.py .)/BuildHostTests/ctest"
cmake -S . -B "$CTEST_BUILD_DIR" -G Ninja -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
ctest --test-dir "$CTEST_BUILD_DIR" -N
ctest --test-dir "$CTEST_BUILD_DIR" -R '^test_vt100_cursor$' --output-on-failure
```

Build outputs live outside the checkout. Resolve their root with
`python3 Scripts/build_root.py .`, or set `UBQ_BUILD_ROOT` explicitly.

A change is ready when the test suite and `check-style.py` both pass and the
affected builds are clean. Hosted CI runs the preflight checks and the secret
scan. The maintainers run the test batteries on their own Linux and macOS
machines before every landing.

## Coding standards

New code meets all three simultaneously: **WebKit C++ guidelines**, **Safe C++
(`CheckedPtr`/`CheckedRef`)**, and **Cocoa/Swift Foundation naming**. See
[`Docs/coding-conventions.md`](../Docs/coding-conventions.md), the reference
every pull request is reviewed against. The codebase is
`-fno-exceptions -fno-rtti`. Names spell words out
(`inputEncoding`, not `inputEnc`); acronyms are ALL-CAPS (`UTF8`). Comments
state present-tense invariants, not history.

`Scripts/check-style.py` enforces formatting (clang-format), the module-boundary
invariants (`check-boundaries.py`), and the naming lints — run it before opening
a PR.

## Pull requests

- One logical change per PR; keep the diff reviewable.
- Include or update tests for behavior changes; update the docs under `Docs/`
  when you change behavior or architecture.
- Make sure CI is green.
- Describe what changed and why, and say what you ran to check it. If the change
  fixes an issue, link it.
- Rebase onto the current `main` immediately before merge
  (`git fetch origin && git rebase origin/main`). Resolve each conflict hunk by
  hunk; never take a whole file from either side, because that silently undoes
  what landed on `main` meanwhile.
- A commit that removes a line landed in the last two weeks by someone else's
  commit names that commit (hash or subject) and says why. The commit-msg hook
  and `Scripts/check-stale-landing.py` enforce it.
- Maintainers merge with `Scripts/merge-pr.sh <number>`: it refuses a PR not
  rebased onto the current tip, runs the stale-landing check, records it as the
  `stale-landing` commit status, and rebase-merges.

## Reporting bugs & proposing features

Open a [bug report or feature
request](https://github.com/ubiquitty/ubiquitty-core/issues/new/choose). A bug report is
most useful with the terminal's version, the OS, and the byte sequence or steps
that reproduce it. For security issues, **do not** open a public issue — see
[`security.md`](security.md).

## Contribution terms

You keep copyright in your work. By accepting these terms for a contribution,
you license your original contribution under
[Apache License 2.0](../LICENSES/Apache-2.0.txt). The unmodified Apache-2.0
text governs the copyright and patent grants, their scope, conditions, and
limitations.

This non-exclusive grant allows Stanislav Yaglo and other recipients to use,
modify, sublicense, and distribute your contribution under Apache-2.0, the
license of every original file in the repository. Apache-2.0's conditions, including applicable license and attribution notices,
still apply. You may use and license your own original work elsewhere. There
is no copyright assignment, exclusivity requirement, or promise of payment.

The grant covers only the rights you can grant in your contribution. It does
not change the license of existing project code or third-party material. You
must have the necessary authority, including from an employer or other rights
holder when applicable. Identify copied or adapted material, its source, and
its license in the pull request, and preserve its notices. Do not describe
someone else's work as your original contribution. Material with different
terms needs explicit maintainer review before inclusion; checking a box cannot
remove those terms.

### Record your agreement

Check the contribution-terms box in the pull request template, or post this
statement in the pull request discussion:

> I accept the contribution terms in `.github/contributing.md` for my original
> contributions in this pull request. I have the right to grant them under
> Apache-2.0, and I have identified any third-party material and its terms.

Each contributor whose original work is included must record their own
agreement, or an authorized representative must identify whom they represent
and confirm their authority. A maintainer must verify the acknowledgements and
any third-party terms before merging. Include a link to the acknowledgement
when applying a patch outside a pull request. A discussion or bug report alone
does not substitute for this acknowledgement when incorporating submitted code.

No external signing service or per-commit trailer is required. This
agreement is the single record of your incoming grant; a `Signed-off-by`
trailer, if you add one out of habit, is neither needed nor a substitute.

### File headers and distribution

Follow the destination file's distribution license. Original files use
`SPDX-License-Identifier: Apache-2.0`; [the licensing guide](../Docs/licensing.md)
names the two exceptions, and `Scripts/check-licenses.py` fails a header that
disagrees with `license-scope.json`.
Third-party material and the Emacs client keep their own identifiers and
notices. Do not change a file's identifier merely because your contribution
has an Apache-2.0 incoming grant: the identifier describes the distributed
file as a whole. Add your attribution where appropriate without removing
others'.

Read [the licensing guide](../Docs/licensing.md) for the policy and the
file-level checks. These terms do not withdraw existing grants
or assert ownership of contributions received under different terms.
