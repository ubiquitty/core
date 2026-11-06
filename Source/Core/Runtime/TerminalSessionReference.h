// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The shared-reference pair for a terminal session, declared once for the
// session's own header and for the escrow that holds a session across a local
// adoption: a pointer either hands out is retained and released through these
// and nothing else.

namespace Ubiquitty::Runtime {
class TerminalSession;
}

/// Claims a reference to `session` on Swift's behalf.
///
/// Declared at global scope because the Swift importer resolves the retain and
/// release named by `UBIQUITTY_SHARED_REFERENCE` there and nowhere else; a
/// namespace-scoped pair is reported as missing rather than as misplaced.
///
/// - Parameter session: The session to retain. A null pointer is ignored.
void retainTerminalSession(Ubiquitty::Runtime::TerminalSession*);

/// Drops a reference to `session`, destroying it at zero.
///
/// - Parameter session: The session to release. A null pointer is ignored.
void releaseTerminalSession(Ubiquitty::Runtime::TerminalSession*);
