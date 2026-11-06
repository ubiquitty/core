// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// QUICChannel.cpp — see QUICChannel.h.

#include "Core/Transport/QUICChannel.h"

#include "Core/Protocol/Sync/ResumeProof.h"

#if defined(UBIQUITTY_QUIC)

#include <algorithm>
#include <span>
#include <utility>

namespace Ubiquitty::Transport {

static_assert(bootstrapTokenPreambleKind == std::to_underlying(Protocol::Sync::PreambleKind::Token), "the carrier leads a token with the kind the daemon reads a token under");

QUICChannel::QUICChannel(std::string host, std::uint16_t port, std::string token, std::string pinnedCertSHA256, Transport::Origin origin)
    : m_host(std::move(host))
    , m_port(port)
    , m_credential(BootstrapToken { .token = std::move(token) })
    , m_pinnedCertSHA256(std::move(pinnedCertSHA256))
    , m_origin(std::move(origin))
{
}

QUICChannel::QUICChannel(std::string host, std::uint16_t port, ExportedPreamble preamble, std::string pinnedCertSHA256, Transport::Origin origin)
    : m_host(std::move(host))
    , m_port(port)
    , m_credential(std::move(preamble))
    , m_pinnedCertSHA256(std::move(pinnedCertSHA256))
    , m_origin(std::move(origin))
{
}

QUICChannel::~QUICChannel() { stop(); }

// The three accessors below are the only places m_client is read or written, each
// holding m_clientMutex for just the pointer copy/swap — never across the call into
// QUICClient (which may join its I/O thread).
std::shared_ptr<QUICClient> QUICChannel::loadClient() const
{
    std::scoped_lock lock(m_clientMutex);
    return m_client;
}

std::shared_ptr<QUICClient> QUICChannel::exchangeClient(std::shared_ptr<QUICClient> next)
{
    std::scoped_lock lock(m_clientMutex);
    m_client.swap(next);
    return next;
}

void QUICChannel::storeClient(std::shared_ptr<QUICClient> next)
{
    std::scoped_lock lock(m_clientMutex);
    m_client = std::move(next);
}

bool QUICChannel::start(ChannelObserver* observer)
{
    if (loadClient())
        return true; // idempotent
    m_hasPeerAuthenticationFailed.store(false, std::memory_order_release);
    auto client = std::make_shared<QUICClient>();
    // Publish before start(): the I/O thread's onConnect fires the instant the
    // handshake completes and may call send() (the HELLO) synchronously, so the
    // client must already be visible to send() when start() spawns that thread.
    storeClient(client);
    m_health.store(Health::Connecting, std::memory_order_release);

    QUICClient::Callbacks callbacks;
    callbacks.onConnect = [this, observer] {
        m_health.store(Health::Connected, std::memory_order_release);
        if (observer)
            observer->onConnect();
    };
    callbacks.onData = [observer](std::span<const std::uint8_t> bytes) {
        if (observer)
            observer->onData(bytes);
    };
    callbacks.onDatagram = [observer](std::span<const std::uint8_t> bytes) {
        if (observer)
            observer->onDatagram(bytes);
    };
    callbacks.onClose = [this, observer, weakClient = std::weak_ptr(client)](Health health, const std::string& reason) {
        // QUICClient already classified the teardown (Closed = graceful, Failed = error); store as-is.
        m_health.store(health, std::memory_order_release);
        if (const auto client = weakClient.lock()) {
            m_hasPeerAuthenticationFailed.store(client->peerAuthenticationFailed(), std::memory_order_release);
            if (const auto code = client->peerApplicationCloseCode()) {
                m_peerApplicationCloseCode.store(*code, std::memory_order_relaxed);
                m_hasPeerApplicationClose.store(true, std::memory_order_release);
            }
        }
        if (observer)
            observer->onClose(reason);
    };

    const auto started = client->start(m_host, m_port, m_credential, m_pinnedCertSHA256, std::move(callbacks));
    if (!started) {
        m_hasPeerAuthenticationFailed.store(started.error() == QUICClient::StartError::MissingCertificatePin, std::memory_order_release);
        m_health.store(Health::Failed, std::memory_order_release);
        storeClient(nullptr);
        return false; // immediate local setup error — no callback per the contract
    }
    return true;
}

void QUICChannel::repath()
{
    if (std::shared_ptr<QUICClient> client = loadClient())
        client->repath();
}

void QUICChannel::send(std::span<const std::uint8_t> data)
{
    // Load a strong ref: it keeps the QUICClient alive for the whole call even if
    // stop() exchanges the member out concurrently on the owner thread.
    if (std::shared_ptr<QUICClient> client = loadClient())
        client->send(data);
}

bool QUICChannel::sendDatagram(std::span<const std::uint8_t> datagram)
{
    std::shared_ptr<QUICClient> client = loadClient();
    return client && client->sendDatagram(datagram);
}

void QUICChannel::stop()
{
    // Exchange the pointer out, then tear down on the local ref OUTSIDE any lock.
    // QUICClient::stop() joins the I/O thread, and a callback on that thread may
    // itself be calling send() (the HELLO path); holding a lock across the join
    // would deadlock. A concurrent send() keeps its own ref, so the instance
    // outlives both calls and its own send/stop synchronization makes them safe.
    if (std::shared_ptr<QUICClient> client = exchangeClient(nullptr))
        client->stop(); // joins the I/O thread; no callback after
    Health expected = m_health.load(std::memory_order_acquire);
    if (expected == Health::Connected || expected == Health::Connecting)
        m_health.store(Health::Closed, std::memory_order_release);
}

std::string QUICChannel::description() const { return "quic " + m_host + ":" + std::to_string(m_port); }

std::optional<std::array<std::uint8_t, 32>> QUICChannel::authenticatedCertificatePin() const
{
    if (health() != Health::Connected || m_pinnedCertSHA256.size() != 32)
        return std::nullopt;
    std::array<std::uint8_t, 32> pin;
    std::copy(m_pinnedCertSHA256.begin(), m_pinnedCertSHA256.end(), pin.begin());
    return pin;
}

bool QUICChannel::queryStats(TransportStats& out) const
{
    // QUICClient fills the shared TransportStats directly (valid=true once sampled).
    std::shared_ptr<QUICClient> client = loadClient();
    return client && client->stats(out);
}

} // namespace Ubiquitty::Transport

#endif // UBIQUITTY_QUIC
