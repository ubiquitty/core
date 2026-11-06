// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Local/Control.h"

#include <string_view>

namespace Ubiquitty::Host {

using Protocol::Local::decodeLocalControlReply;
using Protocol::Local::encodeLocalControlReply;
using Protocol::Local::encodeLocalReloadRequest;
using Protocol::Local::localControlPreamble;
using Protocol::Local::LocalControlReader;
using Protocol::Local::LocalControlReply;
using Protocol::Local::localControlReplyBytes;
using Protocol::Local::localControlRequestBytes;
using Protocol::Local::LocalControlStatus;
using Protocol::Local::localReloadOperation;
using Protocol::Local::localReloadReplyOperation;

/// Facts established by the transport owner, never by the request's bytes.
struct LocalControlPeer {
    bool isSameUserLocalStream = false;
    bool isTrustedOwnerBinary = false;
    bool hasRemoteGreeting = false;
    bool hasDescriptors = false;
};

/// Admits only an owner's first request on its authenticated local connection.
[[nodiscard]] constexpr bool admitsLocalControl(const LocalControlPeer& peer)
{
    return peer.isSameUserLocalStream && peer.isTrustedOwnerBinary
        && !peer.hasRemoteGreeting && !peer.hasDescriptors;
}

/// Rechecks transport authority immediately before the store is reloaded.
template<typename Reload>
[[nodiscard]] LocalControlReply handleLocalReload(const Protocol::Local::LocalControlRequest& request, std::uint64_t daemonInstanceID,
    const LocalControlPeer& peer, Reload&& reload)
{
    LocalControlReply reply { .correlation = request.correlation, .daemonInstanceID = daemonInstanceID };
    if (request.operation == Protocol::Local::LocalControlOperation::ReloadCredentials
        && request.correlation && daemonInstanceID && admitsLocalControl(peer)) {
        reload();
        reply.status = LocalControlStatus::Reloaded;
    }
    return reply;
}

/// The live listener's facts, read before any token is minted.
struct LocalQUICListener {
    bool isEnabled = false;
    std::uint16_t port = 0;
    std::string_view certificateSHA256;
};

/// Whether the complete bounded answer already has admitted output storage.
enum class LocalReplyAdmission : std::uint8_t {
    Refused,
    Reserved
};

/// Checks an append without allowing its byte sum to overflow.
[[nodiscard]] constexpr bool fitsClientOutput(std::size_t pendingBytes, std::size_t additionalBytes, std::size_t capacity)
{
    return pendingBytes < capacity && additionalBytes <= capacity - pendingBytes;
}

/// Mints only after fresh transport authority, a complete listener, and reply admission.
template<typename Mint>
[[nodiscard]] Protocol::Local::LocalQUICTokenReply handleLocalQUICToken(std::uint64_t correlation, std::uint64_t daemonInstanceID,
    const LocalControlPeer& peer, const LocalQUICListener& listener, LocalReplyAdmission admission, Mint&& mint)
{
    using namespace Protocol::Local;
    LocalQUICTokenReply reply { .correlation = correlation, .daemonInstanceID = daemonInstanceID };
    if (!correlation || !daemonInstanceID || !admitsLocalControl(peer) || admission != LocalReplyAdmission::Reserved)
        return reply;
    if (!listener.isEnabled) {
        reply.status = LocalQUICTokenStatus::Disabled;
        return reply;
    }
    if (!listener.port || listener.certificateSHA256.size() != certificateSHA256Length)
        return reply;
    const auto token = mint();
    if (token.empty() || token.size() > maxQUICTokenLength)
        return reply;
    reply.status = LocalQUICTokenStatus::Granted;
    reply.port = listener.port;
    reply.tokenLength = static_cast<std::uint8_t>(token.size());
    for (std::size_t index = 0; index < token.size(); ++index)
        reply.token[index] = static_cast<std::uint8_t>(token[index]);
    for (std::size_t index = 0; index < reply.certificateSHA256.size(); ++index)
        reply.certificateSHA256[index] = static_cast<std::uint8_t>(listener.certificateSHA256[index]);
    return reply;
}

} // namespace Ubiquitty::Host
