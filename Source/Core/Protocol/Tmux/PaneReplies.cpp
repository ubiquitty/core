// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Tmux/PaneReplies.h"

#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"

#include <array>

namespace Ubiquitty::Protocol::Tmux {

void flushPaneReplies(Terminal& terminal, const TmuxClient* client, int paneID, PAL::ByteStream& stream)
{
    if (!client || client->state == TmuxState::Passthrough) {
        return;
    }
    std::array<char, 128> reply;
    std::array<char, 4096> command;
    int length;
    while ((length = drainOutput(&terminal, { reply.data(), reply.size() })) > 0) {
        const int encoded = wrapKeyForPane(client, paneID, { reply.data(), static_cast<std::size_t>(length) }, command);
        if (encoded <= 0 || stream.writeAll({ command.data(), static_cast<std::size_t>(encoded) }) < 0) {
            return;
        }
    }
}

} // namespace Ubiquitty::Protocol::Tmux
