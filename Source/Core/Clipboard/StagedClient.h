// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Clipboard/ReadStage.h"
#include "PAL/TerminalClients.h"

namespace Ubiquitty::Clipboard {

/// Serves approved captures without accessing a frontend's native clipboard.
///
/// One instance belongs to one terminal. Core serializes read requests for
/// that terminal; the frontend stages its capture before resolving a request.
/// The stage owns payloads across the frontend and ingest threads.
class StagedClient : public ClipboardClient {
public:
    /// Consumes the approved text capture, or refuses an unstaged read.
    ///
    /// - Parameter output: Receives bytes, truncated to its capacity.
    /// - Returns: The copied count, or -1 if unstaged or output is empty.
    [[nodiscard]] int readClipboard(ClipboardBuffer, std::span<char> output) final
    {
        return output.empty() ? -1 : m_readStage.serveBytes(output);
    }

    /// Copies approved bytes into the terminal's one-shot stage.
    ///
    /// - Parameter bytes: The capture; empty stages a successful empty reply.
    void provideClipboardReadBytes(ClipboardBuffer, std::span<const char> bytes) final
    {
        m_readStage.stageBytes(bytes);
    }

    /// Copies typed payloads before their frontend-owned storage expires.
    ///
    /// - Parameter items: Borrowed payloads; empty clears the typed stage.
    void provideClipboardReadItems(ClipboardBuffer, std::span<const ClipboardItem> items) final
    {
        stageItems(items);
    }

    /// Moves an owned native capture into the typed stage.
    ///
    /// - Parameter snapshot: The owned capture; empty clears the typed stage.
    void provideSnapshot(Snapshot&& snapshot);

    /// Lists the approved capture's MIME names without consuming it.
    ///
    /// - Parameter output: Receives whole, space-separated MIME names.
    /// - Returns: The copied count, or -1 if no typed capture is staged.
    [[nodiscard]] int readClipboardTypes(ClipboardBuffer, std::span<char> output) final
    {
        return m_readStage.serveTypes(output);
    }

    /// Reads a bounded payload from the approved capture without consuming it.
    ///
    /// - Parameters:
    ///   - mime: The requested MIME name, matched without case or parameters.
    ///   - output: Receives bytes, truncated to its capacity.
    /// - Returns: The copied count, or -1 if the capture has no matching item.
    [[nodiscard]] int readClipboardItem(ClipboardBuffer, std::string_view mime, std::span<char> output) final
    {
        return m_readStage.serveItem(mime, output);
    }

    /// Requires frontend capture even when policy permits the read automatically.
    ///
    /// - Returns: True; native access never runs on the ingest thread.
    [[nodiscard]] bool requiresAsynchronousClipboardRead() final { return true; }

private:
    /// Builds an owned snapshot from borrowed payloads.
    ///
    /// - Parameter items: The payloads to copy into the typed stage.
    void stageItems(std::span<const ClipboardItem> items);
    ReadStage m_readStage;
};

} // namespace Ubiquitty::Clipboard
