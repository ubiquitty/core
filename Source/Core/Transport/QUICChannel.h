// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A Channel over Transport::QUICClient: the migration-capable daemon data plane
// (TLS 1.3 + connection migration). Thin wrap adapting QUICClient's callbacks to
// ChannelObserver + Health. Built only under -DUBIQUITTY_QUIC.

#include "Core/Transport/Channel.h"

#if defined(UBIQUITTY_QUIC)

#include "Core/Transport/QUICClient.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace Ubiquitty::Transport {

class QUICChannel final : public Channel {
public:
    /// Creates a QUIC carrier to one host.
    ///
    /// - Parameters:
    ///   - host: The daemon host to reach.
    ///   - port: Its QUIC port.
    ///   - token: The auth token presented on connect.
    ///   - pinnedCertSHA256: The certificate fingerprint the peer must match.
    ///   - origin: Where this carrier reaches; returned by `origin()`.
    QUICChannel(std::string host, std::uint16_t port, std::string token, std::string pinnedCertSHA256, Transport::Origin);

    /// Creates a QUIC carrier that authenticates with a preamble built from
    /// what its own handshake exports, as a resume proof is, in place of a
    /// bootstrap token.
    ///
    /// - Parameters:
    ///   - host: The daemon host to reach.
    ///   - port: Its QUIC port.
    ///   - preamble: The exporter label, and what builds the preamble.
    ///   - pinnedCertSHA256: The certificate fingerprint the peer must match.
    ///   - origin: Where this carrier reaches; returned by `origin()`.
    QUICChannel(std::string host, std::uint16_t port, ExportedPreamble preamble, std::string pinnedCertSHA256, Transport::Origin);
    ~QUICChannel() override;

    bool start(ChannelObserver*) override;
    /// Returns the pin only after QUIC verified the peer and connected.
    [[nodiscard]] std::optional<std::array<std::uint8_t, 32>> authenticatedCertificatePin() const override;
    [[nodiscard]] bool peerAuthenticationFailed() const override { return m_hasPeerAuthenticationFailed.load(std::memory_order_acquire); }
    /// Returns the application error code the daemon closed with, copied from
    /// the QUIC client when it reported the close.
    [[nodiscard]] std::optional<std::uint64_t> peerApplicationCloseCode() const override
    {
        if (!m_hasPeerApplicationClose.load(std::memory_order_acquire))
            return std::nullopt;
        return m_peerApplicationCloseCode.load(std::memory_order_relaxed);
    }
    /// Hands the bytes to the QUIC client, which owns the outbound buffering.
    ///
    /// Silently drops them when no client is up — before `start()` and after
    /// `stop()` — so a racing sender does not have to hold the lifetime.
    ///
    /// - Parameter data: Bytes the client copies before the call returns.
    void send(std::span<const std::uint8_t> data) override;
    /// Hands one datagram to the QUIC client.
    ///
    /// - Parameter datagram: One whole payload, copied before the call returns.
    /// - Returns: False when no client is up or the client refuses it; see
    ///   `QUICClient::sendDatagram`.
    [[nodiscard]] bool sendDatagram(std::span<const std::uint8_t> datagram) override;
    void stop() override;
    /// Returns the origin stamped at construction.
    ///
    /// - Returns: The channel's origin.
    Transport::Origin origin() const override { return m_origin; }
    Health health() const override { return m_health.load(std::memory_order_acquire); }
    std::string description() const override;
    bool queryStats(TransportStats& out) const override;
    /// Tells the QUIC client its interface is gone, so it moves the
    /// connection to a fresh path; nothing before the carrier starts.
    void repath() override;

private:
    // The Channel's cross-thread send/stop seam is lifetime-safe: m_client is a
    // shared_ptr read only under m_clientMutex. send() copies a strong ref (which
    // keeps the QUICClient alive for the whole call) and releases the lock before
    // calling into it; stop() swaps the ref out under the lock and tears down on
    // its own ref OUTSIDE the lock — so a sender can never dereference a client
    // stop() reset, and the lock is never held across QUICClient::stop()'s
    // I/O-thread join, which a callback-driven send() could otherwise deadlock
    // against. The QUICClient's own send/stop synchronization makes the two calls
    // safe to overlap on the shared instance. (std::atomic<shared_ptr> would be
    // cleaner but libc++ rejects it — shared_ptr is not trivially copyable.)
    [[nodiscard]] std::shared_ptr<QUICClient> loadClient() const;
    [[nodiscard]] std::shared_ptr<QUICClient> exchangeClient(std::shared_ptr<QUICClient> next);
    void storeClient(std::shared_ptr<QUICClient> next);

    mutable std::mutex m_clientMutex;
    std::shared_ptr<QUICClient> m_client;
    std::string m_host;
    std::uint16_t m_port;
    QUICCredential m_credential;
    std::string m_pinnedCertSHA256;
    Transport::Origin m_origin;
    std::atomic<Health> m_health { Health::Idle };
    std::atomic<bool> m_hasPeerAuthenticationFailed { false };
    std::atomic<bool> m_hasPeerApplicationClose { false };
    std::atomic<std::uint64_t> m_peerApplicationCloseCode { 0 };
};

} // namespace Ubiquitty::Transport

#endif // UBIQUITTY_QUIC
