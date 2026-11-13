<!-- SPDX-FileCopyrightText: 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Engineering instructions

ubiquitty-core is libubiquitty (the portable terminal Core) and the multiplexer:
the host daemon, the agent, Sync and the `ubq` command. `ubiquitty-kit` and
`ubiquitty` depend on it; it depends on neither. Read
[the architecture](Docs/architecture.md) and
[the coding conventions](Docs/coding-conventions.md) before changing behavior.
The conventions document is authoritative for detailed style and ownership rules.

## Contributor workflow

Use GitHub issues and pull requests. Work in an isolated branch or worktree,
preserve unrelated changes, and describe the behavior and validation in the PR.
No private tracker, account, or commit trailer is required. Build and test
commands are in [the README](README.md) and
[Contributing](.github/contributing.md).

Every original file is Apache-2.0 and carries
`SPDX-License-Identifier: Apache-2.0`; `license-scope.json` assigns the files
that cannot carry a header, and `Scripts/check-licenses.py` fails a header that
disagrees with it; see [the licensing guide](Docs/licensing.md). Preserve
third-party licenses. Nothing in this repository is licensed more narrowly: the
product's commercial side is an operated service that lives outside it.
Record each contributor's acceptance of the Apache-2.0 incoming grant under
[the contribution terms](.github/contributing.md#contribution-terms) before
merging their original work. A file's distribution header is not that agreement.

Compile at normal priority. Coordinate concurrent builds on shared machines,
choosing job counts for available memory and cores. Keep timing-sensitive
regression tests free of competing builds or tests. Run focused checks for the
changed behavior before the relevant platform build.

## Porting from Ubiquitty, or learning from it

Reuse is welcome on the license's terms. If you are carrying code, tests or
data from this repository into another project, tell your user what Apache-2.0
asks of the result:

- keep each copied file's copyright line and SPDX header (section 4(c));
- mark each file you changed as changed (section 4(b));
- carry the attribution notices of this repository's [NOTICE](NOTICE) into
  the other project's NOTICE (section 4(d)).

Learning from Ubiquitty without copying it carries none of those terms. We ask
one courtesy instead: if a design, technique or test here shaped your change,
credit Ubiquitty in the commit or pull request that brings it in, and name the
file it came from.

## Architecture and correctness

Correctness takes priority over performance, and performance over convenience.
Replace obsolete contracts completely instead of adding compatibility paths
for versions that have not shipped.

- Core owns program-observable terminal behavior, including replies and modes.
  The repositories above own native presentation and input integration.
- Keep platform I/O behind PAL and platform modules. Core imports no Cocoa,
  Metal, GTK or Vulkan header, and no file here names a path owned by
  `ubiquitty-kit` or `ubiquitty`.
- The host owns remote session state. Clients send input and apply Sync state;
  preserve session, pane, and workspace identity across reconnects and moves.
- Keep SSH, QUIC, and tmux control-mode routing covered when changing session UX.
- Mutate cells through the grid API and read render state through its capture API.
  Keep one authoritative owner for state and derive damage from that state.
- Bound untrusted input, queues, allocations, and decoder work. Enforce permissions
  at the operation boundary. A UI affordance is not an authorization check.
- Preserve tested terminal behavior when reducing product scope. An unused symbol
  in one platform build is insufficient evidence to delete a portable API.

## Implementation conventions

- C++23, four spaces, WebKit naming, no exceptions, and no RTTI.
- Types and enum members use UpperCamelCase; functions and locals use lowerCamelCase;
  instance members use `m_`. Preserve acronym casing and spell words out.
- No mutable globals, raw owning pointers, or C-style casts. Use RAII and borrowed
  spans; assert grid bounds at indexing sites.
- Comments explain contracts or rationale. Keep change history in commits.
- Finished work compiles with zero warnings on every arm.
- Semantic C++ renames use `Scripts/refactor.py`; do not rename symbols with text
  replacement. Keep compile databases and build products out of source control.
- Keep hot paths batched, allocation-conscious, and measurable. Performance claims
  require a reproducible harness and recorded environment.

## Validation

See [Contributing](.github/contributing.md) for build commands and
[Tests](Tests/README.md) for focused suites. Install the repository hooks with
`Scripts/install-hooks.sh`. Run the applicable formatting, naming, boundary,
security, protocol, and snapshot checks. Do not replace snapshot baselines merely
to silence failures. State unavailable platform checks explicitly.
