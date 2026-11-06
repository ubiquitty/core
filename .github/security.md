<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Security policy

## Reporting a vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

Email **[yaglo@me.com](mailto:yaglo@me.com)** to report a vulnerability privately.
Email does not require a GitHub account.

If the repository's **Security** tab offers **Report a vulnerability**, GitHub's
private reporting form is also available. Use email when that action is absent.

Please include:

- a description of the issue and its impact,
- steps to reproduce (a minimal byte stream, escape sequence, or config that
  triggers it is ideal — Ubiquitty parses untrusted terminal output),
- the affected component (Core parser, the `ubiquitty-host` daemon + its
  consent/token model, the Sync wire protocol, or a frontend).

You can expect an acknowledgement within a few days. Once a fix is ready we will
coordinate disclosure with you.

## Scope

The most security-relevant surfaces are: the VT/OSC/DCS parser (it consumes
untrusted program output), the `ubiquitty-host` daemon's attach/consent and
capability-token model, and the Core↔client Sync protocol decoder. Memory-safety
or input-validation issues in those paths are in scope.

## Trust boundary

The daemon trusts the operating-system account it runs as. A process already
running as that user can start the daemon's own attach bridge, issue itself a
token, or edit the files under `~/.ubiquitty`, so the agent and consent tiers
are not a boundary against same-user code: they separate programs the daemon
can tell apart by kernel-verified binary identity, by token, or by the label
they declare. A report that needs code already running as the user is out of
scope. A report in which a remote peer, a token of narrower scope, or a
labelled or unrecognised local program gains more than its tier allows is in
scope.
