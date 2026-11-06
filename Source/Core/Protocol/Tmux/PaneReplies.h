// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace Ubiquitty {
struct Terminal;
namespace PAL {
class ByteStream;
}
namespace Protocol::Tmux {
struct TmuxClient;

/// Drains fixed and bulk terminal replies into pane-addressed tmux input.
///
/// The caller holds the pane's model lock. Every chunk retains the same
/// pane identity, independent of the currently focused pane.
/// - Parameters:
///   - terminal: The model whose queued replies are consumed.
///   - client: The primary control client; null or inactive leaves replies queued.
///   - paneID: The originating pane's server identifier.
///   - stream: The primary's control channel, which queues unwritten bytes.
void flushPaneReplies(Terminal& terminal, const TmuxClient* client, int paneID, PAL::ByteStream& stream);

} // namespace Protocol::Tmux
} // namespace Ubiquitty
