// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Protocol/Sync/QUICBootstrap.cpp — see QUICBootstrap.h.
//

#include "Core/Protocol/Sync/QUICBootstrap.h"

#include "Core/Protocol/Local/Control.h"
#include "PAL/Poll.h"
#include "PAL/Socket.h"
#include "PAL/Stream.h"

#include <array>
#include <chrono>
#include <memory>
#include <span>

namespace Ubiquitty::Protocol::Sync {

namespace {

std::uint64_t nowMilliseconds()
{
    // steady_clock rather than clock_gettime: the deadline only needs to advance
    // monotonically, and the standard one has that guarantee on every backend
    // this core is meant to build for.
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

} // namespace

bool requestQUICToken(const std::string& socketPath, std::uint16_t& port, std::string& token, std::string& certSHA256, std::string& error)
{
    QUICTokenRefusal refusal = QUICTokenRefusal::Unreachable;
    return requestQUICToken(socketPath, port, token, certSHA256, error, refusal);
}

bool requestQUICToken(const std::string& socketPath, std::uint16_t& port, std::string& token, std::string& certSHA256, std::string& error, QUICTokenRefusal& refusal)
{
    port = 0;
    token.clear();
    certSHA256.clear();
    error.clear();
    // Unreachable until an answer proves otherwise: every early exit below is
    // a request that never got one, and only the daemon's own status byte may
    // promote a refusal to a stated one.
    refusal = QUICTokenRefusal::Unreachable;
    PAL::LocalSocketError reason = PAL::LocalSocketError::Failed;
    std::unique_ptr<PAL::PollableByteStream> stream = PAL::connectLocal(socketPath, &reason);
    if (!stream) {
        // The backend owns the path-length limit, so the reason carries what a
        // caller-side constant could only guess at.
        error = std::string(PAL::describeLocalSocketError(reason)) + ": " + socketPath;
        return false;
    }

    if (!PAL::peerIsSameUser(stream->handle())) {
        error = "daemon peer is not the current user";
        return false;
    }
    constexpr std::uint64_t correlation = 1;
    const auto request = Local::encodeLocalQUICTokenRequest(correlation);
    if (!request) {
        error = "invalid local token request";
        return false;
    }
    // PAL's character stream transmits the packet's byte representation.
    if (stream->writeAll(std::span<const char>(reinterpret_cast<const char*>(request->data()), request->size())) < 0) {
        error = "write to daemon failed";
        return false;
    }

    Local::LocalQUICTokenReplyReader reader(correlation);
    const std::uint64_t deadline = nowMilliseconds() + 3000;
    for (;;) {
        const std::uint64_t now = nowMilliseconds();
        const int timeout = (now >= deadline) ? 0 : static_cast<int>(deadline - now);
        PAL::Watch watch { &stream->handle(), PAL::Ready::Readable, PAL::Ready::None };
        const int ready = PAL::waitForReadiness(std::span<PAL::Watch>(&watch, 1), timeout);
        if (ready == 0) {
            // An interrupted wait also reports 0, which is why the deadline
            // rather than the return value decides whether time is up.
            if (nowMilliseconds() >= deadline) {
                error = "daemon did not answer in time";
                return false;
            }
            continue;
        }
        if (ready < 0) {
            error = "wait failed";
            return false;
        }
        // One extra byte exposes a coalesced over-bound or trailing packet.
        std::array<char, Local::maxLocalQUICTokenReplyBytes + 1> buffer;
        const int n = stream->read(buffer);
        if (n <= 0) {
            error = "daemon closed before granting a token";
            return false;
        }
        // The local reader interprets the received character buffer as wire bytes.
        const auto state = reader.consume({ reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(n) });
        if (state == Local::LocalQUICTokenReplyReader::State::Refused) {
            error = "malformed local token reply";
            return false;
        }
        if (state != Local::LocalQUICTokenReplyReader::State::Ready)
            continue;
        const auto reply = reader.take();
        if (!reply) {
            error = "missing local token reply";
            return false;
        }
        if (reply->status == Local::LocalQUICTokenStatus::Disabled) {
            refusal = QUICTokenRefusal::Disabled;
            error = "QUIC is not enabled on this daemon";
            return false;
        }
        if (reply->status != Local::LocalQUICTokenStatus::Granted) {
            error = "daemon refused a QUIC bootstrap token";
            return false;
        }
        refusal = QUICTokenRefusal::None;
        port = reply->port;
        token.assign(reply->token.begin(), reply->token.begin() + reply->tokenLength);
        certSHA256.assign(reply->certificateSHA256.begin(), reply->certificateSHA256.end());
        return true;
    }
}

} // namespace Ubiquitty::Protocol::Sync
