// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// How the terminal introduces itself to programs it launches: TERM, plus the
// TERM_PROGRAM / TERM_PROGRAM_VERSION pair applications sniff for capabilities.
// One concept, so one type — PAL stamps these into a child's environment and
// does not care where the values came from.
//
// Its own header rather than a member of Stream.h: this is launch INPUT, not
// part of the transport, and Settings needs the type without wanting the Stream
// interface. Three string views and nothing else, so including it costs nothing.
//
// The caller owns the storage and it must outlive the launch call — the strings
// are copied into fixed buffers before the fork, because reading through a view
// is not async-signal-safe in the child. An empty member is not exported;
// an empty `term` falls back to $UBIQUITTY_TERM.

#include <string_view>

namespace Ubiquitty::PAL {

struct TerminalIdentity {
    std::string_view term;
    std::string_view program;
    std::string_view programVersion;
};

} // namespace Ubiquitty::PAL
