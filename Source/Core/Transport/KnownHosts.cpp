// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Transport/KnownHosts.h"

#include "Core/Codec/Base64.h"
#include "Core/Crypto/SHA256.h"

#include <array>
#include <vector>

namespace Ubiquitty::Transport {

std::string hostKeyFingerprint(std::span<const std::uint8_t> keyBytes)
{
    std::array<unsigned char, 32> digest {};
    Crypto::sha256({ reinterpret_cast<const unsigned char*>(keyBytes.data()), keyBytes.size() }, digest);

    std::vector<char> encoded(Codec::base64EncodedLength(digest.size()));
    const auto written = Codec::base64Encode(
        { reinterpret_cast<const char*>(digest.data()), digest.size() }, encoded);
    if (!written || written->empty())
        return {};

    std::string fingerprint = "SHA256:";
    fingerprint.append(written->data(), written->size());
    // OpenSSH prints the digest unpadded; 32 bytes always leaves one '=' pair.
    while (!fingerprint.empty() && fingerprint.back() == '=')
        fingerprint.pop_back();
    return fingerprint;
}

std::string hostKeyEndpoint(std::string_view host, int port)
{
    std::string endpoint(host);
    endpoint.push_back(':');
    endpoint += std::to_string(port);
    return endpoint;
}

HostKeyVerdict verifyHostKeyFingerprint(std::string_view pinnedFingerprint, std::string_view presentedFingerprint)
{
    if (pinnedFingerprint.empty())
        return HostKeyVerdict::Unknown;
    // A presented fingerprint that failed to compute is empty, and an empty
    // string must never compare equal to a pin.
    if (presentedFingerprint.empty())
        return HostKeyVerdict::Mismatched;
    return pinnedFingerprint == presentedFingerprint ? HostKeyVerdict::Matched : HostKeyVerdict::Mismatched;
}

} // namespace Ubiquitty::Transport
