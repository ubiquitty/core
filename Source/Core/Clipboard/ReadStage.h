// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Clipboard {

/// One captured payload: a MIME name and the bytes it held at capture time.
struct SnapshotItem {
    std::string mime;
    std::vector<char> bytes;
};

/// A typed picture of a pasteboard at one moment, bounded per item.
///
/// The value a deferred read serves from: types and bytes answer from here,
/// so what a requester sees is what the approval saw, even when the
/// pasteboard changes under it.
class Snapshot {
public:
    /// The captured MIME names, space-separated — the seam's type-list shape.
    ///
    /// - Parameter out: Receives the list.
    /// - Returns: The count written; 0 for an empty capture.
    [[nodiscard]] int typeList(std::span<char> out) const;

    /// One captured payload, without copying it.
    ///
    /// The one place a requested name is matched against a captured one, so
    /// that a caller reading whole payloads and one filling a bounded buffer
    /// cannot disagree about which names match. A caseless compare against the
    /// name with its parameters stripped: `image/png;base64` asks for the same
    /// payload as `IMAGE/PNG`.
    ///
    /// - Parameter mime: The requested name.
    /// - Returns: The payload, or nullptr when the capture holds no such name.
    ///   Valid while the snapshot is.
    [[nodiscard]] const SnapshotItem* find(std::string_view mime) const;

    /// One captured payload's bytes.
    ///
    /// - Parameters:
    ///   - mime: The requested name, matched by ``find``.
    ///   - out: Receives the bytes, truncated to fit.
    /// - Returns: The count, or -1 when the capture holds no such name.
    [[nodiscard]] int item(std::string_view mime, std::span<char> out) const;

    /// Whether the capture holds any payload at all.
    [[nodiscard]] bool isEmpty() const { return m_items.empty(); }

    /// Adds one payload; the capture functions build snapshots through this.
    ///
    /// - Parameters:
    ///   - mime: The payload's bare MIME name.
    ///   - bytes: The payload, owned by the snapshot from here on.
    void add(std::string mime, std::vector<char> bytes);

    /// The captured payloads, in capture order.
    [[nodiscard]] std::span<const SnapshotItem> items() const { return m_items; }

private:
    std::vector<SnapshotItem> m_items;
};

/// The staging a deferred clipboard read is served from, shared by both
/// shells' clients: the frontend's approval stages on the UI thread, Core
/// reads on the ingest thread, and the lock in here is the whole handoff.
///
/// The untyped bytes are one-shot — served once and consumed, staged-empty
/// answering 0 where nothing-staged answers -1, the contract the seam's
/// readClipboard states. The typed capture persists until replaced or
/// cleared: one approval's snapshot answers the type list and every item
/// read of the completion it was staged for.
class ReadStage {
public:
    /// Stages the untyped bytes for the next serveBytes. UI thread.
    ///
    /// - Parameter bytes: The capture; empty stages an empty read.
    void stageBytes(std::span<const char> bytes);

    /// Stages a typed capture, replacing any previous one. UI thread.
    ///
    /// - Parameter snapshot: The capture; an empty one clears the typed
    ///   staging, so serves fall back to the bytes.
    void stageTyped(Snapshot&& snapshot);

    /// Serves and consumes the staged bytes — the seam's readClipboard.
    ///
    /// - Parameter out: Receives the bytes.
    /// - Returns: The count, 0 for a staged empty capture, -1 when nothing
    ///   is staged.
    [[nodiscard]] int serveBytes(std::span<char> out);

    /// Serves the typed capture's names — the seam's readClipboardTypes.
    ///
    /// - Parameter out: Receives the space-separated list.
    /// - Returns: The count, or -1 when no typed capture is staged.
    [[nodiscard]] int serveTypes(std::span<char> out);

    /// Serves one typed payload — the seam's readClipboardItem.
    ///
    /// - Parameters:
    ///   - mime: The requested name.
    ///   - out: Receives the bytes.
    /// - Returns: The count, or -1 when the capture serves no such name.
    [[nodiscard]] int serveItem(std::string_view mime, std::span<char> out);

private:
    std::mutex m_lock;
    std::vector<char> m_bytes;
    bool m_hasBytes = false;
    Snapshot m_typed;
    bool m_hasTyped = false;
};

} // namespace Ubiquitty::Clipboard
