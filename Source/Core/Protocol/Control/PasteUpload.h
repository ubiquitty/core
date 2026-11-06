// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Messages.h"

#include <algorithm>
#include <memory>
#include <new>
#include <span>

namespace Ubiquitty::Protocol::Control {

/// The bounded owner's outcome after one raw chunk.
enum class PasteUploadProgress : std::uint8_t {
    Refused,
    More,
    Complete,
};

/// One bounded raw owner, admitted before credit and never a host file.
class PasteUpload {
public:
    /// Reserves exactly the declared body after the owner's authority/budget query.
    /// The caller is serialized with all other owners and includes heldBytes()
    /// in every subsequent admission. The query must not mutate/reenter this owner.
    /// - Parameters:
    ///   - request: The already parsed immutable manifest.
    ///   - transfer: Fresh nonzero credit on this physical connection.
    ///   - admits: Pure authority and aggregate-capacity check before allocation.
    /// - Returns: Whether the new owner exists and credit may be sent.
    template<typename Admits>
    [[nodiscard]] bool begin(PastePushRequest request, std::uint32_t transfer, Admits admits)
    {
        if (m_storage || !transfer || !PastePushDetail::valid(request) || !admits(request.bytes))
            return false;
        std::unique_ptr<std::uint8_t[]> bytes(new (std::nothrow) std::uint8_t[request.bytes]);
        if (!bytes)
            return false;
        m_request = std::move(request);
        m_transfer = transfer;
        m_storage = std::move(bytes);
        m_offset = 0;
        m_nextIndex = 0;
        m_hasCredit = false;
        m_isComplete = false;
        return true;
    }

    /// Records credit only after its exact Notify was admitted to output.
    /// Merely reserving an ID never authorizes a guessed incoming chunk.
    /// - Parameter transfer: The reserved transfer that was actually credited.
    /// - Returns: Whether that still-active owner was credited without rewinding it.
    [[nodiscard]] bool grantCredit(std::uint32_t transfer)
    {
        if (!m_storage || m_isComplete || transfer != m_transfer)
            return false;
        m_hasCredit = true;
        return true;
    }

    /// Revalidates authority before each copy and publishes only a complete hash match.
    /// Invalid input releases the owner; no partial bytes can become a landing.
    /// - Parameters:
    ///   - chunk: One existing canonical raw BulkChunk.
    ///   - admits: Pure current-pane/attachment/Drive-authority check.
    /// - Returns: More, complete, or refused without retaining partial data.
    template<typename Admits>
    [[nodiscard]] PasteUploadProgress accept(const Sync::BulkChunk& chunk, Admits admits)
    {
        const auto refuse = [&] {
            clear();
            return PasteUploadProgress::Refused;
        };
        if (!m_storage || !m_hasCredit || m_isComplete || !admits() || chunk.transfer != m_transfer || chunk.index != m_nextIndex
            || chunk.bytes.empty() || chunk.bytes.size() > Sync::bulkChunkPayloadBytes
            || chunk.compression != Sync::Compression::None || chunk.inflatedBytes
            || chunk.bytes.size() > m_request.bytes - m_offset)
            return refuse();
        std::copy(chunk.bytes.begin(), chunk.bytes.end(), m_storage.get() + m_offset);
        m_offset += chunk.bytes.size();
        ++m_nextIndex;
        if (!chunk.isLast)
            return m_offset == m_request.bytes ? refuse() : PasteUploadProgress::More;
        if (m_offset != m_request.bytes || chunk.sha256 != m_request.sha256)
            return refuse();
        std::array<std::uint8_t, 32> digest;
        Crypto::sha256({ m_storage.get(), m_request.bytes }, digest);
        if (digest != m_request.sha256)
            return refuse();
        m_isComplete = true;
        return PasteUploadProgress::Complete;
    }

    /// Releases any incomplete/complete raw owner on loss, unbind or cancellation.
    void clear()
    {
        m_storage.reset();
        m_request = {};
        m_transfer = 0;
        m_offset = 0;
        m_nextIndex = 0;
        m_hasCredit = false;
        m_isComplete = false;
    }

    [[nodiscard]] bool isActive() const { return static_cast<bool>(m_storage); }
    [[nodiscard]] bool isComplete() const { return m_isComplete; }
    [[nodiscard]] std::size_t heldBytes() const { return m_storage ? m_request.bytes : 0; }
    [[nodiscard]] std::uint32_t transfer() const { return m_transfer; }
    [[nodiscard]] const PastePushRequest& request() const { return m_request; }
    /// Borrows only the complete verified payload while this owner is alive.
    [[nodiscard]] std::span<const std::uint8_t> data() const { return m_isComplete ? std::span<const std::uint8_t>(m_storage.get(), m_request.bytes) : std::span<const std::uint8_t> {}; }

private:
    PastePushRequest m_request;
    std::unique_ptr<std::uint8_t[]> m_storage;
    std::uint32_t m_transfer = 0;
    std::size_t m_offset = 0;
    std::uint32_t m_nextIndex = 0;
    bool m_hasCredit = false;
    bool m_isComplete = false;
};

/// Whether exact upload credit belongs to this source and preserves its cursor.
enum class PasteCreditDisposition : std::uint8_t {
    Unrelated,
    Accepted,
    Repeated,
    Refused,
};

/// One immutable outgoing owner shared by native and Web callers. Only
/// credited raw chunks are offered, one per pass after interactive output.
class PasteUploadSource {
public:
    /// Bounded manifest/correlation/serialized-Call owners and their copies;
    /// the source's admission includes this charge, not only the raw payload.
    static constexpr std::size_t metadataReservationBytes = 8u * Sync::maxFrameBodyBytes;
    /// Captures bytes once after local capacity admission, before issuing Call.
    /// - Parameters:
    ///   - pane: The exact bound handle on this physical connection.
    ///   - mime: The original MIME, including the supported generic empty MIME.
    ///   - requestID: Its nonempty connection-unique string correlation.
    ///   - bytes: The borrowed local payload, copied only after bounds/admission.
    ///   - admits: Pure retained-source capacity query before allocation.
    /// - Returns: Whether this owner and its bounded request were constructed.
    template<typename Admits>
    [[nodiscard]] bool begin(std::uint32_t pane, std::string_view mime, std::string_view requestID,
        std::span<const std::uint8_t> bytes, Admits admits)
    {
        if (m_storage || !pane || bytes.empty() || bytes.size() > maxPastePushBytes
            || !PastePushDetail::text(mime, maxPasteMimeBytes) || requestID.empty()
            || !PastePushDetail::text(requestID, Sync::maxFrameBodyBytes))
            return false;
        PastePushRequest request { pane, std::string(mime), static_cast<std::uint32_t>(bytes.size()) };
        // A digest always has the same 64-character encoding: preflight the
        // complete escaped Call before allocating any payload-sized owner.
        if (pastePushRequest(request, requestID).empty() || !admits(bytes.size() + metadataReservationBytes))
            return false;
        std::unique_ptr<std::uint8_t[]> storage(new (std::nothrow) std::uint8_t[bytes.size()]);
        if (!storage)
            return false;
        std::copy(bytes.begin(), bytes.end(), storage.get());
        Crypto::sha256({ storage.get(), bytes.size() }, request.sha256);
        m_request = std::move(request);
        m_requestID = requestID;
        m_call = pastePushRequest(m_request, m_requestID);
        m_storage = std::move(storage);
        m_transfer = 0;
        m_offset = 0;
        m_nextIndex = 0;
        return true;
    }

    /// Accepts only the original call's exact credit; a repeat never rewinds.
    /// - Parameter ready: A complete canonical notification.
    /// - Returns: Unrelated, new credit, identical repeat, or a conflicting credit.
    [[nodiscard]] PasteCreditDisposition credit(const PasteReady& ready)
    {
        if (!m_storage || ready.requestID != m_requestID || ready.pane != m_request.pane)
            return PasteCreditDisposition::Unrelated;
        if (!ready.transfer || (m_transfer && m_transfer != ready.transfer))
            return PasteCreditDisposition::Refused;
        if (m_transfer)
            return PasteCreditDisposition::Repeated;
        m_transfer = ready.transfer;
        return PasteCreditDisposition::Accepted;
    }

    /// Offers one bounded raw chunk, advancing only after queue admission.
    /// The callback borrows this owner and must not mutate/reenter it.
    /// - Parameter send: Returns true only after the canonical connection queued it.
    /// - Returns: Whether one chunk was admitted; false never skips bytes/index.
    template<typename Send>
    [[nodiscard]] bool sendOne(Send send)
    {
        if (!m_storage || !m_transfer || m_offset == m_request.bytes)
            return false;
        const auto length = std::min(Sync::bulkChunkPayloadBytes, m_request.bytes - m_offset);
        const Sync::BulkChunk chunk { .transfer = m_transfer, .index = m_nextIndex, .isLast = m_offset + length == m_request.bytes, .bytes = std::span<const std::uint8_t>(m_storage.get(), m_request.bytes).subspan(m_offset, length), .sha256 = m_request.sha256 };
        if (!send(chunk))
            return false;
        m_offset += length;
        ++m_nextIndex;
        return true;
    }

    /// Drops all old-connection state; cancellation never replays a landing.
    void clear()
    {
        m_storage.reset();
        m_request = {};
        m_requestID.clear();
        m_call.clear();
        m_transfer = 0;
        m_offset = 0;
        m_nextIndex = 0;
    }

    [[nodiscard]] bool isActive() const { return static_cast<bool>(m_storage); }
    [[nodiscard]] bool hasBytesToSend() const { return m_storage && m_transfer && m_offset < m_request.bytes; }
    [[nodiscard]] std::size_t heldBytes() const { return m_storage ? m_request.bytes : 0; }
    [[nodiscard]] std::size_t retainedBytes() const { return heldBytes() + m_request.mime.capacity() + m_requestID.capacity() + m_call.capacity(); }
    [[nodiscard]] std::size_t queuedBytes() const { return m_offset; }
    [[nodiscard]] const PastePushRequest& request() const { return m_request; }
    [[nodiscard]] const std::string& requestID() const { return m_requestID; }
    [[nodiscard]] const std::string& call() const { return m_call; }

private:
    PastePushRequest m_request;
    std::unique_ptr<std::uint8_t[]> m_storage;
    std::string m_requestID;
    std::string m_call;
    std::uint32_t m_transfer = 0;
    std::size_t m_offset = 0;
    std::uint32_t m_nextIndex = 0;
};

} // namespace Ubiquitty::Protocol::Control
