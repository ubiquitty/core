// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>

namespace Ubiquitty::Host {

/// Runs `ubiquitty-host resume-record <write|read|purge> PATH`, the record
/// store of a client that keeps no Core in its process (Docs/sync.md §6.9).
///
/// `write` reads a `PictureRequest` (Core/Session/ResumeRecordFormat.h) from
/// standard input and stores the record it describes; a request Core refuses
/// purges the stored record, since the Welcome it follows consumed that
/// record's secret. `read` writes the stored record's `ResumeReply` to
/// standard output. `purge` removes it. PATH is an absolute file in a private
/// directory the client created, never a path a peer named.
///
/// - Parameter arguments: The arguments after `resume-record`.
/// - Returns: 0 on success, 1 when the record was refused or none is stored,
///   2 on a usage or storage error.
int runResumeRecordTool(std::span<char*> arguments);

}
